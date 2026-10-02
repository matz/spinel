# A class of the program named into the Errno module reads its parent's
# errno, as CRuby reads the inherited Errno constant.
module Errno
  class MineNotFound < Errno::ENOENT; end
end
class Errno::MineDenied < Errno::EACCES; end
e = Errno::MineNotFound.new("x")
p e.errno, e.message, e.is_a?(Errno::ENOENT)
p Errno::MineDenied.new.errno, Errno::MineDenied.new("y").message
begin
  raise Errno::MineDenied, "z"
rescue SystemCallError => err
  p err.errno, err.message
end
