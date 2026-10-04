# A method called on a value of more than one class, whose default reads an
# earlier parameter that a closure in the method captures: activesupport's
# ErrorReporter#report, `severity: handled ? :warning : :error`, with
# `handled` read inside a block. The dispatch binds each argument to a
# local for a later default to read; the default reads a captured parameter
# through its heap cell, which that local never had, and the C did not
# compile. A constructor reached through a class value binds the same way.
class Rep
  def report(err, handled: true, severity: handled ? :warning : :error)
    later = -> { [err, handled, severity] }
    later.call
  end
end

class Other
  def report(err, handled: false, severity: :x) = [err, handled, severity]
end

x = [Rep.new, Other.new][ARGV.size]
p x.report("e")
p x.report("e", handled: false)

class Box
  def initialize(v, label = "box #{v}")
    @show = -> { "#{label}=#{v}" }
  end
  def show = @show.call
end

class Crate
  def initialize(v, label = "crate") = @v = "#{label}:#{v}"
  def show = @v
end

k = [Box, Crate][ARGV.size]
p k.new(1).show
p k.new(2, "two").show
