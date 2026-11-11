# A top-level def is a private method of Object from where it runs: a call
# with an explicit receiver in a method body that runs after the def raises
# the private NoMethodError, one that runs before every def the undefined
# one (a program that does not reopen Object)
def foo = :top
def upcase = :top

class Shout
  def upcase = "SHOUT"
end

class Mute
end

def try
  yield
rescue NoMethodError => e
  puts e.message
end

def via(v) = v.foo
def either(v) = v.upcase

try { via(5) }
try { via("s") }
[1, :s].each { |v| try { via(v) } }
p either("abc")
p either(Shout.new)
try { either(Mute.new) }

def later(v) = v.late
try { later(1) }
def late = :late
