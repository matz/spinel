# An include a reopening runs after the class's first body: until it runs
# a call reaches what the class answered (the superclass's method), after
# it the module's version of the name, whose super reaches past the module.
module V
  def a = 1
  def a = [super, 2]
end
class P; def a = :p; end
class K < P; end
p K.new.a
class K; include V; end
p K.new.a
