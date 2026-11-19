# Multi-argument system calls execute the command directly with its argv list.

system("ruby", "-e", "puts 'stmt_ok'")

ok = system("ruby", "-e", "exit 0")
puts ok
puts($? == 0)

bad = +"x\0y"
def later_argument
  puts "evaluate later argument"
  ""
end
begin
  system(bad, later_argument)
rescue ArgumentError => error
  puts error.message
end

class LaterConversion
  def initialize(first)
    @first = first
  end

  def to_str
    @first.replace("changed\0tail")
    "end"
  end
end
first = +"initial"
arguments = ["ruby", "-e", "puts ARGV.join(' ')", first, LaterConversion.new(first)]
system(*arguments)
p first
arguments = ["ruby", "-e", "", bad]
begin
  ok = system(*arguments)
rescue ArgumentError => error
  puts error.message
end

ok = system("ruby", "-e", "exit 1")
puts ok
puts($? == 0)

ok = system("ruby", "-e", "exit(ARGV[0] == 'ok' ? 0 : 1)", "ok")
puts ok
puts($? == 0)
