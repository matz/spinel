# A computed send handing keywords alone (`**h`) to a name a builtin Socket
# answers too: the Socket arm, which has no address, raises ArgumentError as
# CRuby does, and the program's own method of the name answers
require "socket"
class Sock
  def connect_nonblock(exception: true) = exception ? :ok : :wait
end
def call_it(r, name, req) = r.__send__(name, **req[0])
objs = [Sock.new, 1, "s"]
req = [ARGV[1] ? { "x" => 1 } : { exception: false }]
p call_it(objs[(ARGV[2] || 0).to_i], (ARGV[0] || "connect_nonblock").to_sym, req)
