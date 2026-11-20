# A tail break leaves through the Fiber/Thread boundary even when an
# enclosing begin uses a boxed result slot. Ensure keeps the exit object.
value = +"fiber"
fiber = Fiber.new do
  begin
    break value
  ensure
    value << "!"
    GC.start
  end
end
begin
  fiber.resume
rescue LocalJumpError => error
  p [error.message, error.exit_value.equal?(value), error.exit_value]
end

Thread.report_on_exception = false
thread_value = +"thread"
thread = Thread.new do
  begin
    break thread_value
  ensure
    thread_value << "!"
    GC.start
  end
end
begin
  thread.value
rescue LocalJumpError => error
  p [error.message, error.exit_value.equal?(thread_value), error.exit_value]
end
