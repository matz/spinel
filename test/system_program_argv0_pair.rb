# system([program, argv0], ...) runs program with argv0 as the argv[0] it
# sees, never through the shell, with or without an environment Hash first.
p system(["true", "x"])
p system({}, ["true", "true-alias"])
p system({"SP_PAIR" => "1"}, ["sh", "named"], "-c", "echo $0 $SP_PAIR")
p system(["sh", "x"], "-c", "exit 3")
p $?.exitstatus
p system(["echo", "x"], "a b")
# the program's name is not split into words
p system(["sh -c", "x"])
p system(["spinel_no_such_program", "x"])
p $?.exitstatus
pair = ["sh", "from_local"]
p system(pair, "-c", "echo $0")
cmd = ARGV.size > 5 ? "echo line" : ["sh", "poly"]
p system(cmd, "-c", "echo $0")
args = ["-c", "echo $0 splat"]
p system(["sh", "splat"], *args)
system(["sh", "statement"], "-c", "echo $0")
begin
  system(["true"])
rescue ArgumentError => e
  p e.message
end
begin
  system([1, "x"])
rescue TypeError => e
  p e.message
end
begin
  system(["true", 2])
rescue TypeError => e
  p e.message
end
begin
  system([])
rescue ArgumentError => e
  p e.message
end

class ReplaceArgv0Slot
  def to_str
    $program_pair[1] = "replacement"
    GC.start
    "sh"
  end
end

class MutateArgv0
  def initialize(argv0)
    @argv0 = argv0
  end

  def to_str
    @argv0.replace("mutated")
    GC.start
    "sh"
  end
end

$program_pair = [ReplaceArgv0Slot.new, +"original"]
p system($program_pair, "-c", 'echo "$0"')
argv0 = +"original"
p system([MutateArgv0.new(argv0), argv0], "-c", 'echo "$0"')
