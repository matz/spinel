# A program class named like an errno macro (EACCES) compiles, and the
# Errno:: path still names the builtin class.
module T
  class EACCES
    def tag = :t
  end
end

class ENOENT
  def tag = :top
end

begin
  File.open("/nonexistent-dir-for-test/x", "w")
rescue Errno::EACCES, Errno::ENOENT => e
  p e.class
end

begin
  raise Errno::EACCES, "x"
rescue Errno::EACCES => e
  p e.class
  p e.message
end

begin
  raise Errno::ENOENT
rescue T::EACCES
  p :wrong
rescue SystemCallError => e
  p e.class
end

p T::EACCES.new.tag
p ENOENT.new.tag
p Errno::EACCES
p Errno::EACCES.ancestors.include?(SystemCallError)
