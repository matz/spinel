module Outside
  def length = 18
end

class Other
  def length = 17
end

class String
  private

  def method_missing(name, *args, &block)
    return [:missing, name, args, block.call(7)] if name == :length
    super
  end
end

def sample(value)
  begin
    p value.length(4) { |n| n + 2 }
  rescue ArgumentError
    p :arity
  end
  p value.respond_to?(:length)
end

value = [+"abc", Other.new][0]
class String
  prepend Outside if ENV["SPINEL_TEST_PREPEND_LAYER"] == "yes"
end
sample(value)

class String
  def length(n) = n + 1
end
sample(value)

class String
  private :length if ENV.fetch("SPINEL_TEST_PREPEND_HIDE", "yes") == "yes"
end
sample(value)
