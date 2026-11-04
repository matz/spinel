# A fiber a hook starts has no $! of its own, as a thread has none. What it
# rescues leaves the hook's $! in place, and a fiber started from a rescue in the
# hook does not read what that rescue handles. The hook's own rescue ends its
# reading of the exception ending the program; the next hook starts from it again.
class E < StandardError; end
at_exit { p [:next_hook, $!] }
at_exit do
  e = $!
  Fiber.new do
    p [:start, $!]
    begin
      raise e
    rescue E
      begin
        raise "inner"
      rescue
      end
      p [:after_nested, $!]
    end
    p [:after_outer, $!]
    begin
      raise "own"
    rescue => x
      p [:cause, x.cause]
    end
    Fiber.yield
    p [:resumed, $!]
  end.tap { |f| $f = f }.resume
  p [:hook_after_fiber, $!]
  $f.resume
  p [:hook_after_resume, $!]
  begin
    raise e
  rescue E
    begin
      raise "inner"
    rescue
    end
    p [:hook_after_nested, $!]
    Fiber.new { p [:fiber_in_rescue, $!] }.resume
    Thread.new { p [:thread_in_rescue, $!] }.join
    p [:hook_in_rescue, $!]
  end
  p [:hook_after_rescue, $!]
end
raise E, "boom"
