/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */
#include <assert.h>

#include <iostream>
#include <sstream>
#include <algorithm>
#include <functional>

#include "BrokerChannel.h"
#include "DeletingTxOp.h"
#include "framing/ChannelAdapter.h"
#include <QpidError.h>
#include <DeliverableMessage.h>
#include <BrokerQueue.h>
#include <BrokerMessage.h>
#include <MessageStore.h>
#include <TxAck.h>
#include <TxPublish.h>
#include "BrokerAdapter.h"
#include "Connection.h"

using std::mem_fun_ref;
using std::bind2nd;
using namespace qpid::broker;
using namespace qpid::framing;
using namespace qpid::sys;


Channel::Channel(
    Connection& con, ChannelId id,
    u_int32_t _framesize, MessageStore* const _store,
    u_int64_t _stagingThreshold
) :
    ChannelAdapter(id, &con.getOutput(), con.client->getProtocolVersion()),
    connection(con),
    currentDeliveryTag(1),
    transactional(false),
    prefetchSize(0),
    prefetchCount(0),
    framesize(_framesize),
    tagGenerator("sgen"),
    store(_store),
    messageBuilder(this, _store, _stagingThreshold),
    opened(true),
    adapter(new BrokerAdapter(*this, con, con.broker))
{
    outstanding.reset();
}

Channel::~Channel(){
    close();
}

bool Channel::exists(const string& consumerTag){
    return consumers.find(consumerTag) != consumers.end();
}

void Channel::consume(string& tag, Queue::shared_ptr queue, bool acks,
                      bool exclusive, ConnectionToken* const connection,
                      const FieldTable*)
{
    if(tag.empty()) tag = tagGenerator.generate();
    ConsumerImpl* c(new ConsumerImpl(this, tag, queue, connection, acks));
    try{
        queue->consume(c, exclusive);//may throw exception
        consumers[tag] = c;
    }catch(ExclusiveAccessException& e){
        delete c;
        throw e;
    }
}

void Channel::cancel(consumer_iterator i){
    ConsumerImpl* c = i->second;
    consumers.erase(i);
    if(c){
        c->cancel();
        delete c;
    }
}

void Channel::cancel(const string& tag){
    consumer_iterator i = consumers.find(tag);
    if(i != consumers.end()){
        cancel(i);
    }
}

void Channel::close(){
    if (isOpen()) {
        opened = false;
        while (!consumers.empty()) 
            cancel(consumers.begin());
        //requeue:
        recover(true);
    }
}

void Channel::begin(){
    transactional = true;
}

void Channel::commit(){
    TxAck txAck(accumulatedAck, unacked);
    txBuffer.enlist(&txAck);
    if(txBuffer.prepare(store)){
        txBuffer.commit();
    }
    accumulatedAck.clear();
}

void Channel::rollback(){
    txBuffer.rollback();
    accumulatedAck.clear();
}

void Channel::deliver(
    Message::shared_ptr& msg, const string& consumerTag,
    Queue::shared_ptr& queue, bool ackExpected)
{
    Mutex::ScopedLock locker(deliveryLock);

    u_int64_t deliveryTag = currentDeliveryTag++;
    if(ackExpected){
        unacked.push_back(DeliveryRecord(msg, queue, consumerTag, deliveryTag));
        outstanding.size += msg->contentSize();
        outstanding.count++;
    }
    //send deliver method, header and content(s)
    msg->deliver(*this, consumerTag, deliveryTag, framesize);
}

bool Channel::checkPrefetch(Message::shared_ptr& msg){
    Mutex::ScopedLock locker(deliveryLock);
    bool countOk = !prefetchCount || prefetchCount > unacked.size();
    bool sizeOk = !prefetchSize || prefetchSize > msg->contentSize() + outstanding.size || unacked.empty();
    return countOk && sizeOk;
}

Channel::ConsumerImpl::ConsumerImpl(Channel* _parent, const string& _tag, 
                                    Queue::shared_ptr _queue, 
                                    ConnectionToken* const _connection, bool ack) : parent(_parent), 
                                                                                    tag(_tag), 
                                                                                    queue(_queue),
                                                                                    connection(_connection),
                                                                                    ackExpected(ack), 
                                                                                    blocked(false){
}

bool Channel::ConsumerImpl::deliver(Message::shared_ptr& msg){
    if(!connection || connection != msg->getPublisher()){//check for no_local
        if(ackExpected && !parent->checkPrefetch(msg)){
            blocked = true;
        }else{
            blocked = false;
            parent->deliver(msg, tag, queue, ackExpected);
            return true;
        }
    }
    return false;
}

void Channel::ConsumerImpl::cancel(){
    if(queue) queue->cancel(this);
}

void Channel::ConsumerImpl::requestDispatch(){
    if(blocked) queue->dispatch();
}

// FIXME aconway 2007-02-05: Drop exchange member, calculate from
// message in ::complete().
void Channel::handlePublish(Message* _message, Exchange::shared_ptr _exchange){
    Message::shared_ptr message(_message);
    exchange = _exchange;
    messageBuilder.initialise(message);
}

void Channel::handleHeader(AMQHeaderBody::shared_ptr header){
    messageBuilder.setHeader(header);
    //at this point, decide based on the size of the message whether we want
    //to stage it by saving content directly to disk as it arrives
}

void Channel::handleContent(AMQContentBody::shared_ptr content){
    messageBuilder.addContent(content);
}

void Channel::handleHeartbeat(boost::shared_ptr<AMQHeartbeatBody>) {
    // TODO aconway 2007-01-17: Implement heartbeating.
}

void Channel::complete(Message::shared_ptr msg) {
    Exchange::shared_ptr exchange =
        connection.broker.getExchanges().get(msg->getExchange());
    assert(exchange.get());
    if(transactional) {
        std::auto_ptr<TxPublish> deliverable(new TxPublish(msg));
        exchange->route(*deliverable, msg->getRoutingKey(),
                        &(msg->getHeaderProperties()->getHeaders()));
        txBuffer.enlist(new DeletingTxOp(deliverable.release()));
    } else {
        DeliverableMessage deliverable(msg);
        exchange->route(deliverable, msg->getRoutingKey(),
                        &(msg->getHeaderProperties()->getHeaders()));
    }
}

void Channel::ack(u_int64_t deliveryTag, bool multiple){
    if(transactional){
        accumulatedAck.update(deliveryTag, multiple);
        //TODO: I think the outstanding prefetch size & count should be updated at this point...
        //TODO: ...this may then necessitate dispatching to consumers
    }else{
        Mutex::ScopedLock locker(deliveryLock);//need to synchronize with possible concurrent delivery
    
        ack_iterator i = find_if(unacked.begin(), unacked.end(), bind2nd(mem_fun_ref(&DeliveryRecord::matches), deliveryTag));
        if(i == unacked.end()){
            throw InvalidAckException();
        }else if(multiple){     
            ack_iterator end = ++i;
            for_each(unacked.begin(), end, mem_fun_ref(&DeliveryRecord::discard));
            unacked.erase(unacked.begin(), end);

            //recalculate the prefetch:
            outstanding.reset();
            for_each(unacked.begin(), unacked.end(), bind2nd(mem_fun_ref(&DeliveryRecord::addTo), &outstanding));
        }else{
            i->discard();
            i->subtractFrom(&outstanding);
            unacked.erase(i);        
        }

        //if the prefetch limit had previously been reached, there may
        //be messages that can be now be delivered
        for(consumer_iterator j = consumers.begin(); j != consumers.end(); j++){
            j->second->requestDispatch();
        }
    }
}

void Channel::recover(bool requeue){
    Mutex::ScopedLock locker(deliveryLock);//need to synchronize with possible concurrent delivery

    if(requeue){
        outstanding.reset();
        std::list<DeliveryRecord> copy = unacked;
        unacked.clear();
        for_each(copy.begin(), copy.end(), mem_fun_ref(&DeliveryRecord::requeue));
    }else{
        for_each(unacked.begin(), unacked.end(), bind2nd(mem_fun_ref(&DeliveryRecord::redeliver), this));        
    }
}

bool Channel::get(Queue::shared_ptr queue, bool ackExpected){
    Message::shared_ptr msg = queue->dequeue();
    if(msg){
        Mutex::ScopedLock locker(deliveryLock);
        u_int64_t myDeliveryTag = currentDeliveryTag++;
        msg->sendGetOk(MethodContext(this, msg->getRespondTo()),
                       queue->getMessageCount() + 1, myDeliveryTag,
                       framesize);
        if(ackExpected){
            unacked.push_back(DeliveryRecord(msg, queue, myDeliveryTag));
        }
        return true;
    }else{
        return false;
    }
}

void Channel::deliver(Message::shared_ptr& msg, const string& consumerTag,
                      u_int64_t deliveryTag)
{
    msg->deliver(*this, consumerTag, deliveryTag, framesize);
}

void Channel::handleMethodInContext(
    boost::shared_ptr<qpid::framing::AMQMethodBody> method,
    const MethodContext& context
)
{
    try{
        method->invoke(*adapter, context);
    }catch(ChannelException& e){
        connection.client->getChannel().close(
            context, e.code, e.toString(),
            method->amqpClassId(), method->amqpMethodId());
        connection.closeChannel(getId());
    }catch(ConnectionException& e){
        connection.client->getConnection().close(
            context, e.code, e.toString(),
            method->amqpClassId(), method->amqpMethodId());
    }catch(std::exception& e){
        connection.client->getConnection().close(
            context, 541/*internal error*/, e.what(),
            method->amqpClassId(), method->amqpMethodId());
    }
}

