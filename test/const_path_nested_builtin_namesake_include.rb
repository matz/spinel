# App::Process includes ::Process, so App::Process::Status resolves to the
# builtin Process::Status.
module App
  module Process
    include ::Process
  end
end
p App::Process::Status
p App::Process::Status == ::Process::Status
