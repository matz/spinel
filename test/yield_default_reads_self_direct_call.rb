# A yielding method whose default reads self is not spliced into a direct
# call: the call takes its proc form, with the call's block or none, as a
# switch arm of a dispatch to the same method does.
class Proxy
  def initialize
    @default_scope = :user
  end

  def authenticated?(scope = @default_scope)
    result = scope == :user
    yield scope if block_given? && result
    result
  end

  def session(scope)
    authenticated?(scope) ? "session of #{scope}" : "none"
  end

  def announce(scope)
    authenticated?(scope) { |s| puts "signed in as #{s}" }
  end
end

class Guest
  def authenticated?(scope) = false
end

def check(who, scope)
  who.authenticated?(scope)
end

proxy = Proxy.new
puts proxy.session(:user)
puts proxy.session(:admin)
p proxy.announce(:user)
p check([proxy, Guest.new][0], :user)
p check([proxy, Guest.new][1], "user")
p proxy.authenticated?

# The proc form answers its parameter boxed (a String reaches it through the
# dispatch); the direct calls take an Integer back, as a switch arm converts.
class Picker
  def initialize
    @default = 5
    @count = 0
  end

  def pick(value = @default)
    yield value if block_given?
    value
  end

  def total(x)
    pick(x) + 1
  end

  def store(x)
    @count = pick(x)
    @count + 1
  end
end

class Zero
  def pick(value) = 0
end

def pick_from(who, value)
  who.pick(value)
end

picker = Picker.new
p picker.total(3)
p picker.store(4)
p pick_from([picker, Zero.new][0], "s")
p pick_from([picker, Zero.new][1], 2)
