# A thread a hook starts has no $! of its own to begin with, and the exception
# ending the program is one it can raise and rescue like any other. Rescuing it
# there and entering a rescue inside that one leaves the outer rescue's $! in
# place, and the hook's $! is untouched by what the thread rescued.
class E < StandardError; end
at_exit { p [:next_hook, $!] }
at_exit do
  e = $!
  Thread.new do
    p [:start, $!]
    begin
      raise e
    rescue E
      begin
        raise "inner"
      rescue
      end
      p [:after_nested, $!]
      begin
        raise
      rescue E => x
        puts "raised again: #{x.message}"
      end
    end
    p [:after_outer, $!]
    begin
      raise "own"
    rescue => x
      p [:cause, x.cause]
    end
  end.join
  p [:hook, $!]
end
raise E, "boom"
