# A handler variable the handler rebinds raises what the write made, not what the clause caught:
# the defs after a call of such a method may not have run.
def fail_load
  begin
    raise "initial"
  rescue RuntimeError => e
    e = LoadError.new("replacement")
    raise e
  end
end
def f = :old
begin
  fail_load
  def f = :new
rescue LoadError
end
p f

def keep_runtime
  begin
    raise "initial"
  rescue RuntimeError => e
    raise e
  end
end
def g = :old
begin
  keep_runtime
  def g = :new
rescue LoadError
rescue RuntimeError
end
p g
