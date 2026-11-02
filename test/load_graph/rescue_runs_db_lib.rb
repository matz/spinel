require_relative "rescue_runs_pragmas_lib"
class RescueRunsDb
  include RescueRunsPragmas
  alias_method :busy_timeout, :busy_timeout=
end
