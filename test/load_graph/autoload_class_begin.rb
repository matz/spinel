class AutoBeginHolder
  begin
    autoload :Z, File.join(__dir__, "autoload_class_begin_lib")
  ensure
    puts "ensure"
  end
end
puts "before"
p AutoBeginHolder::Z
