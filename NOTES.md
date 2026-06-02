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