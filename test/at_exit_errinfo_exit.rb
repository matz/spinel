# `exit` reads as SystemExit in an at_exit hook, also where no frame is armed
# around it and it ends the program at once. An exit from inside a rescue body
# reads as the SystemExit, not as the exception that rescue handled.
at_exit do
  puts "#{$!.class} status=#{$!.status} success=#{$!.success?} message=#{$!.message}"
  puts "is a SystemExit: #{$!.is_a?(SystemExit)}"
  puts "cause: #{$!.cause.inspect}"
end
begin
  raise "handled"
rescue
  exit 7
end
