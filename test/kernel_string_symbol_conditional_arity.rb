# Kernel.String invokes the selected conversion with no arguments.
def convert(value)
  p String(value)
rescue => error
  p [error.class, error.message]
end

convert(:abc)
if ARGV.empty?
  class Symbol
    def to_str(required)
      +"the body must not run"
    end
  end
end
convert(:abc)
begin
  p String(:abc)
rescue => error
  p [error.class, error.message]
end
