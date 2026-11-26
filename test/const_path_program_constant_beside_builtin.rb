# a namespace's own constant named like a builtin is read lexically ahead of
# the builtin, and a builtin path's leaf is read under that path only: a
# program module Constants nested in another class is not File::Constants
module Flags
  RDONLY = 7
end

module App
  IO = Flags
  def self.flag = IO::RDONLY
  p IO::RDONLY
end
p App::IO::RDONLY, App.flag

class Sock
  module Constants
    RDONLY = 44
  end
  def self.r = Constants::RDONLY
end
p File::Constants::RDONLY, File::RDONLY, Sock.r, Sock::Constants::RDONLY
