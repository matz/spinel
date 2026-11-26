# IO#readpartial with nothing buffered takes at most n bytes from the
# descriptor, as CRuby does: the rest of a pipe stays there for another
# reader, such as a child process that inherits the descriptor. A second
# handle on the same descriptor shows what the first one left.
r, w = IO.pipe
w.write "1\n2\n3\n"
w.close
p r.readpartial(1)
p IO.for_fd(r.fileno, autoclose: false).read

# what a handle already buffered (gets) is served first and stays its own
r, w = IO.pipe
w.write "ab\ncd\nef\n"
w.close
p r.gets
p r.readpartial(2), r.readpartial(100)
p IO.for_fd(r.fileno, autoclose: false).read

# a read longer than what the pipe holds answers what is there
r, w = IO.pipe
w.write "xyz"
p r.readpartial(1000)
w.close
begin
  r.readpartial(1)
rescue EOFError => e
  puts e.message
end
