# A Fiber argument can still belong to the caller of the enclosing method.
def m(s)
  f = Fiber.new { |t| t << "!" }
  f.resume(s)
  s = nil
end
x = +"a"
m(x)
p x
