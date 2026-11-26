# a Constants a class other than File assigns through self is that class's:
# the File::Constants rewrite stays on, and the compiler reads the write's
# self parent as no File (it ended with SIGSEGV on that node)
module Flags
  RDONLY = 7
end

class Sock
  self::Constants = Flags
  def self.r = Constants::RDONLY
end

p Sock::Constants::RDONLY, Sock.r, File::Constants::RDONLY
