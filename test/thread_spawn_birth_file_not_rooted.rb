# Thread.new passes the call's file name to the runtime as a bare C literal,
# kept for Thread#inspect. sp_Thread_spawn_fiber_at rooted it as a String
# across the allocation of the thread record, so a collection there read the
# byte in front of the literal to find the String's kind: a read outside the
# literal, which AddressSanitizer reports as a global-buffer-overflow (make
# rt-san-check). A byte there that happened to be a String marker would have
# been taken for a heap String. The name is rodata and nothing marks it.
#
# The eight captured Arrays make the block's capture struct, allocated just
# before the thread record, large enough to be the allocation that starts a
# collection; TCPServer.new puts an instrumented literal in front of the file
# name in the generated C, so the stray read lands in a redzone.
require "socket"

def spawn(i)
  server = TCPServer.new("127.0.0.1", 0)
  a = [i]; b = [i]; c = [i]; d = [i]; e = [i]; f = [i]; g = [i]; h = [i]
  Thread.new { server.close; a + b + c + d + e + f + g + h }.value
end

sum = 0
Thread.new { 300.times { |i| sum += spawn(i).size } }.join
puts sum
puts Thread.new {}.inspect.include?("thread_spawn_birth_file_not_rooted.rb")
