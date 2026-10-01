at_exit { puts "at_exit" }
def helper
  yield
ensure
  puts "method ensure"
end
begin
  begin
    pr = proc { return }
    helper { pr.call }
    puts "not reached"
  rescue Exception => e
    puts "rescued #{e.class}"
  ensure
    puts "inner ensure"
  end
ensure
  puts "outer ensure"
end
puts "not reached either"
