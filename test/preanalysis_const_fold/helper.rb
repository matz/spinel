PREANALYSIS_REQUIRED_CONST = :loaded_from_helper

# Verify pre-analysis pruning works inside required files (even without -D)
if RUBY_PLATFORM.include?("wasi")
  require "spinel_nonexistent_helper_wasi_gem"
  helper_dead = :never
end
PREANALYSIS_HELPER_DEAD_LOCAL = [helper_dead, defined?(helper_dead)]

