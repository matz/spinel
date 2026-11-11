# Kernel#format and #sprintf with no format string raise ArgumentError.
# The call was refused instead, and under --defer-refusals the refusal took
# its whole statement. A boxed receiver's `x.format` reaches that call too:
# where the program reads Kernel as a value, the call gets an arm for
# `x.equal?(Kernel)` that runs Kernel.format, made bare; the refused arm
# took the program's own `format` with it. actionview's TemplateRenderer
# calls `template.format` on a boxed template beside `include Kernel`.
class A
  def format = :a
  def sprintf = "s"
end

class B
  def format = 1
end

def pick(k) = { a: A.new, b: B.new, n: 1 }[k]

t = pick(:a)
p t.format
p t.sprintf
p pick(:b).format
p Kernel

[-> { format }, -> { format() }, -> { sprintf }, -> { Kernel.format }, -> { Kernel.sprintf }].each do |f|
  f.call
rescue ArgumentError => e
  p [e.class, e.message]
end

p format("%d-%s", 1, "x")
p Kernel.sprintf("%05.1f", 3.14159)
