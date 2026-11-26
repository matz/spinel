# A subclass of a builtin that defines its own method_missing keeps the
# builtin's methods when the program also reopens that builtin: the chain's
# root is then the reopened String or Array, and the methods above it are
# still String's or Array's. `replace` (what `super` in initialize runs),
# `length` and `<<` are answered there, the reopening's own method too, and
# only a name no ancestor has reaches method_missing.
class String
  def shout = upcase + "!"
end

class Inquirer < String
  private
    def respond_to_missing?(name, include_private = nil)
      name.end_with?("?") || super
    end

    def method_missing(name, ...)
      return self == name.to_s.chomp("?") if name.end_with?("?")
      raise NotImplementedError, "mm #{name}"
    end
end

class EnvInquirer < Inquirer
  def initialize(env)
    raise ArgumentError, "'local' is a reserved environment name" if env == "local"
    super(env)
    @development = env == "development"
  end

  def development? = @development
end

env = EnvInquirer.new(ENV["SPINEL_TEST_ENV"] || "development")
p env.length
p env.to_s
p env.shout
p env.development?
p env.test?
env << "-x"
p env.to_s

class Array
  def second = self[1]
end

class Bag < Array
  def method_missing(name, *args)
    return size if name == :count_them
    raise NotImplementedError, "mm #{name}"
  end
end

b = Bag.new
b << 1
b.push(2, 3)
p b.length
p b.second
p b.count_them
p b.first
