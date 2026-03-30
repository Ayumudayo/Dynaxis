if (Python3_Interpreter_FOUND)
    add_test(
      NAME StackPythonPluginScriptAcceptance
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_plugin_script_matrix.py"
              --scenario runtime-on-acceptance
    )
    add_test(
      NAME StackPythonVerifyFpsPhase2Acceptance
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_fps_rudp_transport_matrix.py"
              --scenario phase2-acceptance
              --no-build
    )
    add_test(
      NAME StackPythonVerifyMmorpgRuntimeClosureMatrix
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_mmorpg_runtime_matrix.py"
              --scenario phase3-acceptance
              --no-build
    )
    add_test(
      NAME StackPythonVerifyContinuityRecoveryMatrix
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/run_stack_test.py"
              "${CMAKE_SOURCE_DIR}/tests/python/verify_continuity_recovery_matrix.py"
              --scenario phase5-recovery-baseline
              --no-build
    )
    add_test(
      NAME KubernetesWorldsLocalDevManifestCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_localdev.py"
    )
    add_test(
      NAME KubernetesWorldsKindLiveCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind.py"
    )
    add_test(
      NAME KubernetesWorldsKindControlPlaneCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_control_plane.py"
    )
    add_test(
      NAME KubernetesWorldsKindClosureCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_closure.py"
    )
    add_test(
      NAME KubernetesWorldsKindContinuityCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_continuity.py"
    )
    add_test(
      NAME KubernetesWorldsKindMultiGatewayCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_multigateway.py"
    )
    add_test(
      NAME KubernetesWorldsKindRestartCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_restart.py"
    )
    add_test(
      NAME KubernetesWorldsKindMetricsBudgetCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_metrics_budget.py"
    )
    add_test(
      NAME KubernetesWorldsKindLocatorFallbackCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_locator_fallback.py"
    )
    add_test(
      NAME KubernetesWorldsKindWorldStateFallbackCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_world_state_fallback.py"
    )
    add_test(
      NAME KubernetesWorldsKindRedisOutageCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_redis_outage.py"
    )
    add_test(
      NAME KubernetesWorldsKindWorkerOutageCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_worker_outage.py"
    )
    add_test(
      NAME KubernetesWorldsKindGatewayIngressImpairmentCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_gateway_ingress_impairment.py"
    )
    add_test(
      NAME KubernetesWorldsKindResumeImpairmentCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_resume_impairment.py"
    )
    add_test(
      NAME KubernetesWorldsKindMultiFaultImpairmentCheck
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/python/verify_worlds_kubernetes_kind_multifault_impairment.py"
    )

    set_tests_properties(
      StackPythonPluginScriptAcceptance
      PROPERTIES
        LABELS "python;stack;plugin-script"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      StackPythonVerifyFpsPhase2Acceptance
      PROPERTIES
        LABELS "python;stack;fps;hardening"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      StackPythonVerifyMmorpgRuntimeClosureMatrix
      StackPythonVerifyContinuityRecoveryMatrix
      PROPERTIES
        LABELS "python;stack;continuity"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsLocalDevManifestCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds"
    )
    set_tests_properties(
      KubernetesWorldsKindLiveCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindControlPlaneCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;control-plane"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindClosureCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;closure"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindContinuityCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;continuity"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindMultiGatewayCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;multigateway"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindRestartCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;restart"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindMetricsBudgetCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;metrics"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindLocatorFallbackCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;fallback"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindWorldStateFallbackCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;fallback"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindRedisOutageCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;outage"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindWorkerOutageCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;outage"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindGatewayIngressImpairmentCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;impairment"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindResumeImpairmentCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;impairment"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindMultiFaultImpairmentCheck
      PROPERTIES
        LABELS "python;kubernetes;worlds;kind;impairment"
        SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE
    )
    set_tests_properties(
      KubernetesWorldsKindLiveCheck
      KubernetesWorldsKindControlPlaneCheck
      KubernetesWorldsKindClosureCheck
      KubernetesWorldsKindContinuityCheck
      KubernetesWorldsKindMultiGatewayCheck
      KubernetesWorldsKindRestartCheck
      KubernetesWorldsKindMetricsBudgetCheck
      KubernetesWorldsKindLocatorFallbackCheck
      KubernetesWorldsKindWorldStateFallbackCheck
      KubernetesWorldsKindRedisOutageCheck
      KubernetesWorldsKindWorkerOutageCheck
      KubernetesWorldsKindGatewayIngressImpairmentCheck
      KubernetesWorldsKindResumeImpairmentCheck
      KubernetesWorldsKindMultiFaultImpairmentCheck
      PROPERTIES
        RESOURCE_LOCK "kind-worlds-cluster"
    )
endif()
