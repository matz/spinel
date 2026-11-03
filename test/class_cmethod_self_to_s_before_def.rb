# A class method that reads its own class by to_s or inspect, bare or on self,
# or a method handed the class, answers the builtin's before the program's
# class method of the name is defined, for the class and for its subclass
# (whose class rides in at run time), and the program's method after it.
# (One def per name, as in class_builtin_before_override.rb.)

class A
  def self.t = self.to_s
  def self.i = inspect
  def self.bare = to_s
end
class B < A; end

def texts(k) = [k.inspect, k.to_s]
def calls = [A.t, B.t, A.i, B.i, A.bare, B.bare, texts(A), texts(B)]
p calls
def A.to_s = "a"
def A.inspect = "ai"
p calls
