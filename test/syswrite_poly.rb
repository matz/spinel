# syswrite_poly.rb -- IO#syswrite on a Socket through the poly dispatch.
#
# The poly arm in codegen_call.c (the switch with SP_BUILTIN_IO cases) is
# only emitted when the receiver is typed TY_POLY at compile time. A plain
# `sock.syswrite(...)` where sock is statically TY_IO takes the typed-
# receiver arm, not the poly arm. To force the poly arm the receiver must
# be a union of a builtin IO (Socket) and a user class -- exactly the
# shape TlsSocket has in http_client.rb, where `sock` may be either a raw
# Socket or a TlsSocket wrapper.
#
# syswrite semantics: unbuffered, writes the whole String in one shot,
# returns the byte count, and does NOT append a newline (unlike IO#write
# on $stdout, which is a different code path). No flush: sp_File_write_bin
# routes straight to write(2) on the descriptor.
#
# The generated C is the proof the poly arm was taken: it emits
#   switch (cls_id) { case 0: <user class>; case SP_BUILTIN_IO: sp_File_write_bin(...); default: raise }
# A plain Socket receiver would emit the typed fast path with no switch.

require "socket"

# A user class that owns #syswrite, mirroring TlsSocket's role: the union
# of this class and Socket makes the receiver poly, and owning the name
# opens the per-class dispatch switch.
class StubSslSocket
  def syswrite(_data); 42; end
end

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]

# Assign a StubSslSocket first, then a Socket to the SAME variable. The
# variable's static type widens to the union (StubSslSocket | Socket), so
# the receiver of .syswrite is TY_POLY and the dispatch switch is emitted.
# At runtime the variable holds the Socket, so the builtin SP_BUILTIN_IO
# arm runs.
holder = StubSslSocket.new
holder = TCPSocket.new("127.0.0.1", port)

# String arg -> sp_File_write_bin, byte count is the operand length.
n = holder.syswrite("hello syswrite")
raise "syswrite returned #{n.inspect}, expected 14" unless n == 14

# Non-String arg (Integer) goes through sp_poly_to_s, writes "42" (2 bytes).
m = holder.syswrite(42)
raise "syswrite(42) returned #{m.inspect}, expected 2" unless m == 2

holder.close
server.close

puts "PASS: syswrite through the poly dispatch works"
