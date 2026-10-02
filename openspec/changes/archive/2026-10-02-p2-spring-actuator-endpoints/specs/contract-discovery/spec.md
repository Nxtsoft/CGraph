## ADDED Requirements

### Requirement: A Spring Boot application serves the Actuator endpoints it exposes
A build file (`build.gradle.kts`, `build.gradle`, `pom.xml`) that applies the Spring Boot plugin (Maven: the Boot parent or `spring-boot-maven-plugin`) and, in the project's own dependencies, puts `org.springframework.boot:spring-boot-starter-actuator` and a servlet or reactive web starter on the runtime classpath SHALL yield a `spring_actuator` node at the actuator dependency. The application config files `application[-<profile>].{yml,yaml,properties}` directly under that module's `src/main/resources` SHALL decide which Actuator web endpoints are served, as Spring Boot does: only `health` by default; `management.endpoints.web.exposure.include` and `exclude` (with `*`, exclude winning); endpoint access (`management.endpoint.<id>.access`, `management.endpoints.access.default`, `access.max-permitted`, the legacy `enabled` booleans; `shutdown` and `heapdump` none by default; read-only serving GET only); `management.endpoints.web.base-path` (default `/actuator`) and `path-mapping`; the context path on the main port or `management.server.base-path` on another management port, nothing on port `-1`; the discovery page at a non-root base path; health's groups and probes. The served endpoints SHALL be the union over the base configuration and each profile overlaid on it, and each SHALL be `handled_by` the `actuator_exposure` node of the document whose `include` won, or the `spring_actuator` node when none sets it. A configuration whose deciding value is unknowable (a placeholder without a default, a non-root dispatcher servlet path) SHALL serve nothing, and a repository that only mentions the actuator outside these files SHALL serve no Actuator endpoint.

#### Scenario: A client of the health endpoint meets its provider
- **GIVEN** a module whose `build.gradle.kts` applies `org.springframework.boot` and depends on `spring-boot-starter-web` and `spring-boot-starter-actuator`, whose `src/main/resources/application.yml` sets `management.endpoints.web.exposure.include: health,info`, and a Kotlin Ktor client calling `client.get("$baseUrl/actuator/health")`
- **THEN** `endpoint:GET /actuator/health` is served, `handled_by` the `actuator_exposure` node at the `include` line, and consumed by the calling function

#### Scenario: Without config only health is exposed
- **GIVEN** the same build, declaring Boot 3.4.0, and no application config
- **THEN** `GET /actuator`, `GET /actuator/health` and `GET /actuator/health/{*path}` are served, handled by the `spring_actuator` node, and nothing else

#### Scenario: Boot 4 serves the probe groups by default
- **GIVEN** the build declares Boot 4.0.1 and no application config
- **THEN** `GET /actuator/health/liveness` and `GET /actuator/health/readiness` are served too; with `management.endpoint.health.probes.enabled: false`, or when the build names no Boot version, they are not

#### Scenario: Management port rules mirror ManagementPortType
- **GIVEN** `server.servlet.context-path: /idp` and `management.server.base-path: /m`
- **THEN** a negative `management.server.port` serves nothing; `8080` with no `server.port`, or a non-zero port equal to `server.port`, serves under `/idp/actuator`; `0` (even with `server.port: 0`) or another port serves under `/m/actuator`; a non-integer port serves nothing

#### Scenario: Unreadable or disabled configurations serve nothing
- **GIVEN** a `management` key whose value is an anchor (`&m`), a tag (`!!map`) or a flow mapping, or `spring.main.web-application-type: none`
- **THEN** no Actuator endpoint is served for that configuration

#### Scenario: A profile exposes more
- **GIVEN** `application.yml` exposing `health,info` and `application-prod.yml` setting `management.endpoints.web.base-path: /ops`
- **THEN** the endpoints under both `/actuator` and `/ops` are served

#### Scenario: Shutdown is not served by a wildcard
- **GIVEN** `management.endpoints.web.exposure.include: "*"`
- **THEN** `POST /actuator/shutdown` and `GET /actuator/heapdump` are not served, `GET /actuator/prometheus` is served only when `micrometer-registry-prometheus` is a dependency, and `GET /actuator/caches` only when `spring-boot-starter-cache` (or `spring-boot-cache`) is

#### Scenario: Mentioning the actuator serves nothing
- **GIVEN** a repository whose only actuator references are a Kubernetes probe path, a README, and a build without Spring Boot; or a build whose actuator is only in a Maven `<profile>`, whose Boot plugin is only in `<pluginManagement>` or applied with `apply false`, or whose packaging is `pom`
- **THEN** no `endpoint`, `spring_actuator` or `actuator_exposure` node is emitted
