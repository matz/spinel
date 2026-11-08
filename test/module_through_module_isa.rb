module Retryable; end
module Mid; include Retryable; end
module Deep; include Mid; end
module Other; end
class AppError < StandardError; include Deep; end
class SubError < AppError; end
class Plain; include Deep; end
class PlainSub < Plain; end
class Bare; end

def kind(v)
  case v
  when Retryable then :ret
  when Other then :other
  else :none
  end
end

begin
  raise SubError, "x"
rescue => e
  p e.is_a?(Retryable), e.is_a?(Mid), e.is_a?(Other), Retryable === e, kind(e)
  p e.class.ancestors.take(5)
end

[AppError, SubError].each do |k|
  begin
    begin
      raise k
    rescue Retryable => r
      puts "rescued #{r.class}"
    end
  rescue => e
    puts "escaped #{e.class}"
  end
end

begin
  begin
    raise ArgumentError
  rescue Retryable
    puts "wrong"
  end
rescue => e
  puts "escaped #{e.class}"
end

o = PlainSub.new
p o.is_a?(Retryable), o.is_a?(Mid), o.is_a?(Other), kind(o)
xs = [Plain.new, PlainSub.new, Bare.new, 1, "s"]
p xs.map { |x| x.is_a?(Retryable) }
p xs.map { |x| Retryable === x }
p xs.map { |x| kind(x) }
p xs.grep(Retryable).size

# a receiver typed as the class: the folded is_a? and a when arm
t = Plain.new
if t.is_a?(Retryable) then puts "typed is_a? yes" else puts "typed is_a? no" end
p(case t when Retryable then :ret else :none end)
p(case t when Other then :other else :none end)
