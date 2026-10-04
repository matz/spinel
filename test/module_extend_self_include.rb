# A module that does `include Mixin` and `extend self` gets the mixin's
# methods on its singleton side too. dry-monads hands out Success and
# Failure through this shape. The bare call inside a module method
# compiled to an instance call on the module object and the C failed to
# build.

module Result
  def Success(value) = [:ok, value]
  def Failure(error) = [:err, error]
  def ok?(result) = result[0] == :ok
end

module App
  include Result
  extend self

  def run(n)
    n > 0 ? Success(n) : Failure("negative")
  end

  def check(n) = ok?(run(n))
end

p App.run(42)
p App.run(-1)
p App.check(1)
p App.check(-1)
p App.Success("direct")

# `extend self` puts every module the module includes on its singleton side,
# also one included after the statement or in a reopening.
module Tags
  def tag(s) = "<#{s}>"
end
module Shout
  def shout(s) = s.upcase
end

module Page
  extend self
  include Tags
  def title(s) = tag(s)
end
module Page
  include Shout
  def heading(s) = shout(title(s))
end

p Page.title("t")
p Page.heading("h")
p Page.shout("direct")
