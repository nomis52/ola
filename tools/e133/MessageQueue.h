/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * MessageQueue.h
 * Copyright (C) 2013 Simon Newton
 *
 * A class to write IOStacks (which we use to represent ACN messages) to
 * ConnectedDescriptors. Each message is added to the queue and then sent when
 * the underlying ConnectedDescriptor is writable.
 *
 * Explaination:
 *  If we just write IOStacks dirrectly to TCPSockets, we may not be able to
 *  write the entire message. This can happen if the remote end is slow to ack
 *  and data builds up in the kernel socket buffer.
 *
 *  This class abstracts the caller from having to deal with this situation. At
 *  construction time we specify the maximum number of message bytes we want to
 *  buffer. Once the buffer reaches this size subsequent calls to SendMessage
 *  will return false.
 */

#ifndef TOOLS_E133_MESSAGEQUEUE_H_
#define TOOLS_E133_MESSAGEQUEUE_H_

#include <ola/io/Descriptor.h>
#include <ola/io/IOQueue.h>
#include <ola/io/IOStack.h>
#include <ola/io/MemoryBlockPool.h>
#include <ola/io/OutputBuffer.h>
#include <ola/io/SelectServerInterface.h>

class MessageQueue {
 public:
  MessageQueue(ola::io::ConnectedDescriptor *descriptor,
               ola::io::SelectServerInterface *ss,
               ola::io::MemoryBlockPool *memory_pool,
               unsigned int max_buffer_size = DEFAULT_MAX_BUFFER_SIZE);
  ~MessageQueue();

  /**
   * @brief Adjust the number of bytes we'll buffer.
   * @param new_limit the new limit in bytes
   *
   * If this is lower than the current amount buffered, the limit will be
   * lowered to the amount in the buffer
   */
  void ModifyLimit(unsigned int new_limit);

  /**
   * @brief Returns true if we've reached the specified maximum buffer size.
   *
   * Once the limit is reached, no new messages will be sent until the buffer
   * drains.
   */
  bool LimitReached() const;
  bool SendMessage(ola::io::IOStack *stack);

  static const unsigned int DEFAULT_MAX_BUFFER_SIZE;

 private:
  ola::io::ConnectedDescriptor *m_descriptor;
  ola::io::SelectServerInterface *m_ss;
  ola::io::IOQueue m_output_buffer;
  bool m_associated;
  unsigned int m_max_buffer_size;

  void PerformWrite();
  void AssociateIfRequired();
};
#endif  // TOOLS_E133_MESSAGEQUEUE_H_
