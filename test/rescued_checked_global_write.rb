# A write of a global CRuby checks (a TypeError for a value of the wrong
# class) or refuses may raise, so a statement after it in a rescued body
# is not sure to have run. These writes hold a value the global takes, so
# nothing raises, the body runs whole, and the def and the undef after
# each take effect as they do in CRuby.
def a = :old
begin
  $stdout = $stdout
  def a = :new
rescue TypeError
  p :type_error
end
p a

def b = :old
begin
  $~ = nil
  def b = :new
rescue TypeError
  p :type_error
end
p b

def c = :old
begin
  $/ = $/
  def c = :new
rescue TypeError
  p :type_error
end
p c

def d = :old
begin
  $0 = $0
  undef d
rescue TypeError
  p :type_error
end
p((d rescue $!.class))

class K; def m = 1; end
begin
  $stderr = $stderr
  class K; def m = 2; end
rescue TypeError
  p :type_error
end
p K.new.m
p K.method_defined?(:m)

def e = :old
begin
  $. = $.
  def e = :new
rescue TypeError
  p :type_error
end
p e
