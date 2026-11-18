thread = Thread.current
first = thread.status
second = thread.status
held = [first, 11]
p [first == "run", first.equal?(second), first.frozen?]
first << "x" * 180
p [held[0].bytesize, second.bytesize]
