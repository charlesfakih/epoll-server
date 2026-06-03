June 1st :

blocking_server.cpp:

By first writing a blocking TCP server, these are the lessons that I learnt while testing it:

- The length passed to recv must leave room for the terminator you add afterward. Pass sizeof(buf) - 1 so that buf[n] = '\0\ stays in bounds. The f. Without the -1, resizing the buffer to a bigger number only changed WHEN the bug triggers, not whether it exists.

It can very well be a silent bug if your inputs are small and if sent slower than recv consumes them, especially in a line-buffered protocol like telnet. An example of the error detected with UBSan and ASan:

[32, 4128) 'buf' (line 16)  <== Memory access at offset 4128 overflows this variable
[4256, 4304) 'hints'

And a snippet of the shadow bytes:

=>0x...df000: 00 00 00 00 [f2] f2 f2 f2 f2 f2 ...

f2 here is a "stack mid redzone" that ASan deliberately inserted to catch this case. The buf's last write would land in that poison. Without ASan in your build, it's possible there would have been no padding, leaving your program running and quietly writing into adjacent stack memory.

- The addrlen argument passed in accept(...) is a value-result parameter and hence must be initialized to a default value before every call. Since accept(...) can deal with many different address types (IPv4, IPv6, etc.) it can't deduce the size from the generic struct sockaddr*, thus you also separately pass an addrlen argument that gets overwritten to the real size. If the address did not fit, accept(...) writes only what fits but sets the addrlen to a size larger than the buffer you gave it, and that signals truncation. To avoid that, you initialize it to the generous size of sockaddr_storage and it will return the size of the real type, say sockaddr_in (16 bytes) if it's IPv4. The next accept(...) is not guaranteed to fit in only 16 bytes, thus you have to reinitialize it at the beginning of the loop in order to avoid truncation.

- results is passed as a double pointer (&results) in getaddrinfo(...) to avoid passing a pointer copy. We want the function to overwrite the original results variable. And since the function performs a malloc, and ownership of the variable is yours, it is incumbent to call freeaddrinfo(results) after you finish binding to the address.

The biggest takeaway from this blocking TCP server is its limitation by design. Since the recv(...) loop is nested inside of the accept(...) loop, you cannot serve more than one client at once. When testing it, I noticed that my second client's outputs only get serviced when the first client's connection is closed.

------------------------------------------------------------------

The epoll_server announces a conceptual shift: instead of committing to waiting for one fd like in the blocking server, you let the kernel tell you which ones are ready. That dissolves the nested-loop problem because you don't need to loop on the clients to wait for them to write. You iterate through the list of ready file descriptors and read the buffers. Crucially, you can do this in a single-threaded server.

Other observations:

- Non-blocking sockets is a prerequisite, not merely a best practice when you have multiple connexions. A blocking recv(...) on one fd freezes the thread entirely, preventing you from servicing any other fd. In the epoll paradigm, EAGAIN is what gives a "fully drained" signal to pass to the next on the ready list, and you need non-blocking sockets for this to happen.

- ET vs LT: Edge Triggered fires an event on transition, i.e. new data arriving, whereas Level Triggered fires an event whenever data exists. In ET, you drain fully on each notification and then go back to sleep. Thus ET reduces the number of epoll_wait wakeup: fewer system calls, lower context switching and less CPU overhead. The tradeoff is that since the event is only fired at the edge, you cannot avoiding draining the I/O buffer until it returns an EAGAIN, otherwise you could end up in a situatino where you have unread data but no new writes on a fd. It's thus silently stranded from teh application's perspective, with no way of knowing it's there.

- When BUF_SIZE is small, the data is split across different recv calls and doesn't trigger an error.

- SO_REUSEADDR must be set before bind() to avoid the "Address already in use" error on the socket.

Limitation of the current state: The server prints to stdout. The clients themselves get nothing back. That's where fan-out steps in. We'll have to write code for the client in order to handle whatever is sent back to it. I wonder if we can do this efficiently with Socket Streams at scale if we don't have access to broadcast. What about partial writes?

-----------------------------------------------------------------

Adding fan-out strategy to the epoll-server requires deciding how to deal with the issue of partial writes, as send(...) is not guaranteed to send the entire buffer to each client, as the kernel's send buffer can be full. To ensure consistency between what every client sees, including ordering, we have set out to store a write buffer and the offset it's reached whenever for every client (std::unordered_map in C++) whenever the server has not sent to the client all its destined data. Whenever the server has sent everything destined to it, the write buffer is cleared. Separately, we must also append any new messages to the unsent part of the old message in order to preserve ordering.

Interesting here is the role of the EPOLLOUT flag that we add as a tracked event to file descriptors to whom we sent partial data.  That way, the epoll_wait can return them as ready to perform another send(...) to, and we just handle them in the loop of the ready list. There, the write buffer that we computed upon the original partial write comes in handy, as we can perform a send with that data and only disarm the EPOLLOUT flag whenever the full buffer has been consumed, otherwise let it continue to fire an event.

A C++ issue that I was attentive to here was to not erase the key of the clients unordered_map while iterating through it. That would cause the infamous iterator invalidation for the rest of the loop. Instead, I kept track of the deleted keys inside a new vector, and after the loop exit, I deleted the fd's in a standalone small loop.


------------------------------------------------------------------------

June 2nd

I wrote a client and here are a few observations:

- connect(...) is the client intiating while a server waits with accept(...). No need for bind(...) or listen(...) call here, since this is not a server.

- The Message struct embeds the seq and timestamp_ns in the payload so the receiver can compute latency easily. Fixed-size messages avoid framing and adding any other size logic. Length is data here.

- We used memcpy(...) to copy the buffer's raw bytes into the Message struct, in order to respect the strict aliasing rules we would have broken with reinterpret_cast. Even though it works in practice, casting a buffer pointer to anything not char* or byte* is UB because the compiler operates with the assumption that this does not happen and thus freely optimizes the code based on that assumption.

- To measure time, we used clock_gettime(CLOCK_MONOTONIC) as it gives one-directional time guarantee insulated from the NTP jumps of a wall clocks. Its resolution is at the nanosecond level. Ideal for latency calculations.

- In the loop, send(...) must be positioned before epoll_wait(...) as no events are fired until the client has sent a message.

- Curiously, timeout=0 gave worse latency than timeout=10. This means the send rate without timeout outpaced the fan-out processing, messages backlogged and queuing delay dominated.

- Here, the client and server are on the same machine. This is a benchmark environment limitation, since they both share CPU and loopback stack and thus compute for resources. Real benchmarks need separate machines.

-----------------------------------------------------------------

Profiling:

- If task-clock is 10% of wall clock, the process was sleeping 90% of the time. CPUs utilized is derived from this. IPC tells you how efficiently the CPU worked when it was running. Low IPC means stalling on memory or not doing much compute, but with 7% utilization and mostly sleeping, this number is not very meaningful. Effective GHz very low = process sleeping frequently. The branch misses % was elevated but the absolute numbers are small enough for it to not be a meaningful bottleneck at this load. The if/else chains in the event loop dispatch are perhaps not predictable to the CPU because it depends on which fd happens to be ready.

Flamegraph:

First flamegraph showed printf dominating even with "no printfs". Stale binary, didn't rebuild. Always rebuild before profiling. You have to be selective about what commenting out, you remove the prints from your hot path. The error handling is a cold path that you don't need to clear for profiling.

After removing printf, __memmove_avx_unaligned_erms dominated. It comes from write_buf.insert(...) in fanout copying message bytes into the per-connection vector for every recipient. There was also a second significant box, the __send syscall overhead.

One proposed fix: reference-counted shared buffers: one copy per message. Each client would hold a pointer and offset. Something similar to shared_ptr but without the atomic overhead.

Latency and throughput:

timeout=0 gave 3ms latency vs sub-millisecond at timeout = 10
The reason is that in the former, the send rate outpaced the fan-out processing, so the whole chain of send buffer, recv buffer, etc. build up and latency explodes.

Latency (duration) and throughput (messages/second) are different units and measure different things. When you're at a load level that is below the "knee of the curve", latency is stable and throughput increases. Above it, the queues back up and latency explodes (as we have seen with the 0 timeout that is assuredly above the knee).

In a real messaging system, measuring p50/p99/p999 is essential. Relying on the average is misleading, it tells you nothing about how the latency is distributed statistically. The average case can have low latency but the occasional spike affects the user-experience as well. For a messaging system, we track the p999 to account for the worst-case user experience.

Again, the limitation here is that client and server are on the same machine. They share CPU and loopback stack. Real benchmarks need separate machines.