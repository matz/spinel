# method_missing and respond_to_missing? defined under a condition not taken
# make no Method: the capture raises NameError.
FLAG = ARGV.size > 0
class Cond
  if FLAG
    def method_missing(name, *args) = name == :a ? :mm : super
    def respond_to_missing?(name, include_private = false) = name == :a || super
  end
end
begin
  Cond.new.method(:a)
rescue NameError => e
  p [e.class, e.name]
end
p Cond.new.respond_to?(:a)
