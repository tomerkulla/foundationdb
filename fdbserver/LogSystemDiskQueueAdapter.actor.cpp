/*
 * LogSystemDiskQueueAdapter.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "IDiskQueue.h"
#include "LogSystem.h"
#include "LogSystemDiskQueueAdapter.h"
#include "Knobs.h"

class LogSystemDiskQueueAdapterImpl {
public:
	ACTOR static Future<Standalone<StringRef>> readNext( LogSystemDiskQueueAdapter* self, int bytes ) {
		while (self->recoveryQueueDataSize < bytes) {
			if (self->recoveryLoc == self->logSystem->getEnd()) {
				// Recovery will be complete once the current recoveryQueue is consumed, so we no longer need self->logSystem
				TraceEvent("PeekNextEnd").detail("Queue", self->recoveryQueue.size()).detail("Bytes", bytes).detail("Loc", self->recoveryLoc).detail("End", self->logSystem->getEnd()); 
				self->logSystem.clear();
				break;
			}

			if(!self->cursor->hasMessage()) {
				wait( self->cursor->getMore() );
				TraceEvent("PeekNextGetMore").detail("Queue", self->recoveryQueue.size()).detail("Bytes", bytes).detail("Loc", self->recoveryLoc).detail("End", self->logSystem->getEnd()); 
				if(self->recoveryQueueDataSize == 0) {
					self->recoveryQueueLoc = self->recoveryLoc;
				}
				if(!self->cursor->hasMessage()) {
					self->recoveryLoc = self->cursor->version().version;
					continue;
				}
			}
			
			self->recoveryQueue.push_back( Standalone<StringRef>(self->cursor->getMessage(), self->cursor->arena()) );
			self->recoveryQueueDataSize += self->recoveryQueue.back().size();
			self->cursor->nextMessage();
			if(!self->cursor->hasMessage()) self->recoveryLoc = self->cursor->version().version;

			//TraceEvent("PeekNextResults").detail("From", self->recoveryLoc).detail("Queue", self->recoveryQueue.size()).detail("Bytes", bytes).detail("Has", self->cursor->hasMessage()).detail("End", self->logSystem->getEnd()); 
		}
		if(self->recoveryQueue.size() > 1) {
			self->recoveryQueue[0] = concatenate(self->recoveryQueue.begin(), self->recoveryQueue.end());
			self->recoveryQueue.resize(1);
		}

		if(self->recoveryQueueDataSize == 0)
			return Standalone<StringRef>();

		ASSERT(self->recoveryQueue[0].size() == self->recoveryQueueDataSize);

		//TraceEvent("PeekNextReturn").detail("Bytes", bytes).detail("QueueSize", self->recoveryQueue.size());
		bytes = std::min(bytes, self->recoveryQueue[0].size());
		Standalone<StringRef> result( self->recoveryQueue[0].substr(0,bytes), self->recoveryQueue[0].arena() );
		self->recoveryQueue[0].contents() = self->recoveryQueue[0].substr(bytes);
		self->recoveryQueueDataSize = self->recoveryQueue[0].size();
		if(self->recoveryQueue[0].size() == 0) {
			self->recoveryQueue.clear();
		}
		return result;
	}
};

Future<Standalone<StringRef>> LogSystemDiskQueueAdapter::readNext( int bytes ) {
	if (!enableRecovery) return Standalone<StringRef>();
	return LogSystemDiskQueueAdapterImpl::readNext(this, bytes);
}

IDiskQueue::location LogSystemDiskQueueAdapter::getNextReadLocation() {
	return IDiskQueue::location( 0, recoveryQueueLoc );
}

IDiskQueue::location LogSystemDiskQueueAdapter::push( StringRef contents ) {
	while(contents.size()) {
		int remainder = pushedData.size() == 0 ? 0 : pushedData.back().capacity() - pushedData.back().size();

		if(remainder == 0) {
			VectorRef<uint8_t> block;
			block.reserve(pushedData.arena(), SERVER_KNOBS->LOG_SYSTEM_PUSHED_DATA_BLOCK_SIZE);
			pushedData.push_back(pushedData.arena(), block);
			remainder = block.capacity();
		}

		pushedData.back().append(pushedData.arena(), contents.begin(), std::min(remainder, contents.size()));
		contents = contents.substr(std::min(remainder, contents.size()));
	}

	return IDiskQueue::location( 0, nextCommit );
}

void LogSystemDiskQueueAdapter::pop( location upTo ) {
	ASSERT( upTo.hi == 0 );
	poppedUpTo = std::max( upTo.lo, poppedUpTo );
}

Future<Void> LogSystemDiskQueueAdapter::commit() {
	ASSERT( !commitMessages.empty() );

	auto promise = commitMessages.front();
	commitMessages.pop_front();

	CommitMessage cm;
	cm.messages = this->pushedData;
	this->pushedData = Standalone<VectorRef<VectorRef<uint8_t>>>();
	cm.popTo = poppedUpTo;
	promise.send(cm);

	return cm.acknowledge.getFuture();
}

Future<Void> LogSystemDiskQueueAdapter::getError() {
	return Void();
}

Future<Void> LogSystemDiskQueueAdapter::onClosed() {
	return Void();
}

void LogSystemDiskQueueAdapter::dispose() {
	delete this;
}

void LogSystemDiskQueueAdapter::close() {
	delete this;
}

Future<LogSystemDiskQueueAdapter::CommitMessage> LogSystemDiskQueueAdapter::getCommitMessage() {
	Promise<CommitMessage> pcm;
	commitMessages.push_back( pcm );
	return pcm.getFuture();
}

LogSystemDiskQueueAdapter* openDiskQueueAdapter( Reference<ILogSystem> logSystem, Tag tag ) {
	return new LogSystemDiskQueueAdapter( logSystem, tag );
}