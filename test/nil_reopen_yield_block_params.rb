# A block given to a yielding method the program defines on NilClass or
# Object, called on nil, takes its parameters' types from what the method
# yields.
class NilClass
  def tap; yield +"s"; end
  def nil_each; yield 1; yield 2; 7; end
  def nil_pair; yield 1.5, :a; end
  def nil_mixed; yield 1; yield "two"; yield nil; end
end
nil.tap { |x| x << "!"; p x }
r = nil.nil_each { |v| p v + 1 }
p r
nil.nil_pair { |f, s| p f * 2; p s }
nil.nil_mixed { |v| p v }
class Object
  def obj_each; yield +"o"; end
end
Object.new.obj_each { |x| x << "!"; p x }
"q".obj_each { |x| x << "?"; p x }
nil.obj_each { |x| x << "?"; p x }
