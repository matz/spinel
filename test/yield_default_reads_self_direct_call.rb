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
