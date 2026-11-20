def leave_through_throw
  $saved_return = proc { |word| return word }
  throw :done, +"escaped"
end

def call_with_live_home
  live_return = proc { |word| return word }
  result = catch(:done) { leave_through_throw }
  begin
    $saved_return.call(+"expired")
  rescue LocalJumpError => error
    p [error.reason, error.exit_value]
  end
  GC.start
  live_return.call(result << "!")
end

p call_with_live_home
