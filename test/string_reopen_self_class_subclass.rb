# In a method the program defines in String, self is a String or an instance
# of a String subclass, and its class is the subclass, typed and boxed.
class String
  def own_class = self.class
  def own_class_name = self.class.name
  def kin = [is_a?(String), kind_of?(String), String === self]
  def shown = inspect
end
class Sub < String; end
class SubSub < Sub; end

s = Sub.new("a")
p s.own_class, s.own_class_name, s.kin, s.shown
ss = SubSub.new("b")
p ss.own_class, ss.own_class_name
p "plain".own_class, "plain".own_class_name

def pick(i) = [Sub.new("c"), "d", SubSub.new("e")][i]
3.times { |i| p pick(i).own_class }
boxed = [s, 1].first
p boxed.own_class, boxed.own_class_name, boxed.kin

# a bare String still builds a String
class String
  def twin = self.class.new("t")
end
p "x".twin.class
