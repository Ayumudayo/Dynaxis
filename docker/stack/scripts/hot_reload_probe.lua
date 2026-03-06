function on_login(ctx)
  if not ctx or not ctx.session_id then
    return { decision = "pass" }
  end

  server.send_notice(ctx.session_id, "hot reload probe")
  server.log_debug("lua hot reload probe fired")
  return { decision = "pass" }
end
