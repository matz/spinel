# instance_exec(*args, &block) forwarding the method's own block, as the
# method's last expression, from call sites whose blocks answer different
# kinds. A bare tail keeps the first site's type, because each inlined site
# returns its own value; a tail that sits in parentheses or in a `begin` is
# not inlined that way, and the value of the call took the first site's
# type as well: another site's String or Array was assigned to an
# Integer slot, and the C did not build. Only the bare tail is the method's
# tail: the others are boxed, as a call whose value is used is.
class Plain
  def initialize(n) = @n = n

  def run(*args, &block)
    instance_exec(*args, &block)
  end
end

class Parens
  def initialize(n) = @n = n

  def run(*args, &block)
    ((instance_exec(*args, &block)))
  end
end

class Deep
  def initialize(n) = @n = n

  def run(*args, &block)
    (((((((((((((((((instance_exec(*args, &block))))))))))))))))))
  end
end

class Guarded
  def initialize(n) = @n = n

  def run(*args, &block)
    begin
      instance_exec(*args, &block)
    end
  end
end

class Ensured
  def initialize(n) = @n = n

  def run(*args, &block)
    begin
      instance_exec(*args, &block)
    ensure
      @n += 0
    end
  end
end

r1 = Plain.new(10)
p r1.run { @n + 1 }
p r1.run(2) { |a| "n=#{@n * a}" }
p r1.run(2, 3) { |a, b| [@n, a, b] }
r2 = Parens.new(10)
p r2.run { @n + 1 }
p r2.run(2) { |a| "n=#{@n * a}" }
p r2.run(2, 3) { |a, b| [@n, a, b] }
r3 = Deep.new(10)
p r3.run { @n + 1 }
p r3.run(2) { |a| "n=#{@n * a}" }
p r3.run(2, 3) { |a, b| [@n, a, b] }
r4 = Guarded.new(10)
p r4.run { @n + 1 }
p r4.run(2) { |a| "n=#{@n * a}" }
p r4.run(2, 3) { |a, b| [@n, a, b] }
r5 = Ensured.new(10)
p r5.run { @n + 1 }
p r5.run(2) { |a| "n=#{@n * a}" }
p r5.run(2, 3) { |a, b| [@n, a, b] }
