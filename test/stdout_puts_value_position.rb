# $stdout/$stderr.puts/print in a value/return position must emit a single C
# expression. The lowering joined fputs(...) and the trailing fputc('\n', ...)
# with a ';', producing a statement sequence — fine in statement position, but
# when the call lands in value position the emitter wraps it as
# `(<here>, sp_box_nil())`, yielding the invalid `(fputs(..); fputc(..), nil)`
# ("expected ')' before ';'"). It must be a comma-operator expression instead.
#
# Trigger: an if/else whose other branch yields a boxed value (the File.open
# block) forces the $stdout.puts branch into a returned-value position — the
# exact shape from tep's Logger#log. (Uses $stdout so output is captured.)
class Out
  def initialize(p)
    @p = p
  end
  def write(line)
    if @p.length > 0
      File.open(@p, "a") { |f| f.puts(line) }
    else
      $stdout.puts(line)
    end
  end
end

Out.new("").write("hello")
$stdout.print("done")
$stdout.puts("")
