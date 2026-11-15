# A raise form is Cls.exception(msg), so new(msg): the message binds to a
# reopened builtin exception's initialize as the one argument of a call
# does, an initialize that cannot take that many arguments raises
# ArgumentError, and a raise's cause: survives a raise the initialize
# rescues.

class RuntimeError
  def initialize(prefix = "p", message)
    begin
      Integer("not a number")
    rescue ArgumentError
    end
    super("#{prefix}:#{message}")
  end
end

class IndexError
  def initialize(message, required)
    super("#{message} #{required}")
  end
end

class ZeroDivisionError
  def initialize
    super("fixed")
  end
end

def noisy(s)
  puts "message ran"
  s
end

begin; raise "boom"; rescue => e; puts e.message; end
begin; raise RuntimeError, "boom"; rescue => e; puts e.message; end

why = TypeError.new("why")
begin; raise "outer", cause: why; rescue => e; puts "#{e.message} cause=#{e.cause.message}"; end
begin; raise RuntimeError, "outer", cause: why; rescue => e; puts "#{e.message} cause=#{e.cause.message}"; end

begin; raise IndexError, noisy("one"); rescue ArgumentError => e; puts "ArgumentError: #{e.message}"; end
begin; raise ZeroDivisionError, noisy("one"); rescue ArgumentError => e; puts "ArgumentError: #{e.message}"; end
begin; raise ZeroDivisionError; rescue ZeroDivisionError => e; puts e.message; end
