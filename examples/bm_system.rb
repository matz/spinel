# Test system features needed for ccm

# ENV
puts ENV['HOME'] != nil  # true

# Dir.home
home = Dir.home
puts home.length > 0  # true

# system()
system("echo hello_from_system")  # hello_from_system

# backtick
result = `echo backtick_test`.strip
puts result  # backtick_test

# trap (just register, don't trigger)
trap('INT') { }
puts "trap set"

# $stdin — skip interactive test
# File.readlink — skip (needs symlink)

puts "done"
