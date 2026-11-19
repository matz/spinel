# A Method of a class method that takes its receiving class runs a default that reads it
# (`v = new`) on the class the Method names, also when the default calls another class method first.
class Helper
  def self.mark = :mark
end
class P
  def self.make(v = new) = v
  def self.made(v = [Helper.mark, new]) = v
end
class Child < P; end
class Other < P; end
p Child.method(:make).call.class
p P.method(:make).call.class
p Other.method(:make).to_proc.call.class
p Child.method(:made).call.map { |x| x.is_a?(Symbol) ? x : x.class }
p Child.method(:make).call(7)
