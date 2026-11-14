# The same def under a condition that ran: the def answers, at either count.
class On < BasicObject
  if ::ARGV.size == 0
    def respond_to_missing?(n, p = false) = n == :x
  end
  def t = respond_to_missing?(:x, false)
  def u = respond_to_missing?(:y)
end

def try
  ::Kernel.p yield
rescue ::NoMethodError
  ::Kernel.p :nme
end

try { On.new.t }
try { On.new.u }
