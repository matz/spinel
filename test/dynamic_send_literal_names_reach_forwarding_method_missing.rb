# A send of a name a variable holds (one of the program's literals) on a
# receiver whose class defines a method_missing that forwards the name by
# __send__: the dispatch's method_missing arm hands the name on as the
# literal it is, so the forwarding send dispatches over the program's
# literals and not over every name a computed one could take.
module Enumerable
  def sole
    first
  end

  def many?
    count > 1
  end
end

class BodyProxy
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

class Forwarder
  def initialize(target)
    @target = target
  end

  def call(*args, &block)
    @target.__send__(*args, &block)
  end
end

bp = BodyProxy.new(ARGV.empty? ? ["a", "b"] : {})
bp.each { |x| puts x }
p bp.sole
n = :sole
p bp.send(n)
f = Forwarder.new(bp)
p f.call(:sole)
