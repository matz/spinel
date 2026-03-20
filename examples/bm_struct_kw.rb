# Test Struct with keyword_init

Session = Struct.new(
  :pid, :name, :status,
  keyword_init: true
)

s = Session.new(pid: 123, name: "test", status: "running")
puts s.pid     # 123
puts s.name    # test
puts s.status  # running

s.status = "done"
puts s.status  # done

puts "done"
