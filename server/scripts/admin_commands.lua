function on_admin_command(ctx)
  if not ctx or ctx.command ~= "announce" then
    return { decision = "pass" }
  end

  local text = ctx.args or ""
  if text == "" then
    return {
      decision = "deny",
      reason = "announce requires text",
    }
  end

  server.log_info("lua announce broadcast requested")
  server.broadcast_all("[announce] " .. text)
  return { decision = "handled" }
end
