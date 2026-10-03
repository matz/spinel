# A program's own IO method that yields has no standalone function for a
# call on an IO to reach (emit_io_reopen_call), so the call is refused. The
# call plan decides it from the reopening's scope (cplan_io_reopen_yields).
class IO
  def twice
    yield
    yield
  end
end
$stdout.twice { puts "hi" }
