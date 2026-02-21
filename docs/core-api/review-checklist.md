# 코어 API PR 리뷰 체크리스트

- [ ] API 범위가 `docs/core-api-boundary.md`의 분류와 일치한다.
- [ ] 소유권/수명주기 계약이 명시적이며 일관된다.
- [ ] 스레드 안전성과 오류 동작이 문서화되어 있다.
- [ ] 호환성 영향이 (`Stable`/`Transitional`/`Internal`)로 분류되어 있다.
- [ ] 파괴적 변경에 대한 마이그레이션 노트가 추가되어 있다.
- [ ] `docs/core-api/` 하위 도메인 문서가 같은 PR에서 갱신되었다.
- [ ] 공개 API 스모크 소비자 빌드가 성공한다.
- [ ] 확장 ABI 영향이 분류되어 있고 호환성 전략이 문서화되어 있다.
- [ ] 확장 ABI 사용 중단/제거 시 명시적 마이그레이션 가이드가 포함되어 있다.
- [ ] Gateway 확장 계약 변경이 `docs/core-api/gateway-extension-interface.md`와 정합적이다.
- [ ] Write-behind 확장 계약 변경이 `docs/core-api/write-behind-extension-interface.md`와 정합적이다.
