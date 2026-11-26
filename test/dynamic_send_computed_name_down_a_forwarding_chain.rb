# A name computed at a call site and handed down five methods into a send
# whose receiver's method_missing forwards it: the method_missing arm's hop
# costs no depth, so the forwarding send still takes the computed name.
class Impl
  def goal
    "goal"
  end

  def other
    "other"
  end
end

class Proxy
  def initialize(body)
    @body = body
  end

  def respond_to_missing?(method_name, include_all = false)
    @body.respond_to?(method_name, include_all)
  end

  def method_missing(method_name, *args, &block)
    @body.__send__(method_name, *args, &block)
  end
end

def r5(px, n)
  px.send(n)
end

def r4(px, n)
  r5(px, n)
end

def r3(px, n)
  r4(px, n)
end

def r2(px, n)
  r3(px, n)
end

def r1(px, n)
  r2(px, n)
end

px = Proxy.new(ARGV.empty? ? Impl.new : Object.new)
p r1(px, ("go" + "al").to_sym)
p r1(px, :other)
