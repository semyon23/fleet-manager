/**
 * Runtime-конфиг веб-UI для раздачи через шлюз fms (deploy/nginx/fms.conf).
 * Кладётся поверх config.js из сборки: /opt/fms/www/fleet-manager/config.js.
 * Адресов машины здесь нет: UI, API и MQTT — с того же адреса, с которого открыт UI.
 * Пароль fleet-ui вписать на месте, в git его не коммитим. Пользователь только на чтение,
 * пароль всё равно виден в браузере (deploy/README.md, раздел про mosquitto).
 */
window.__FLEET_CONFIG__ = {
  apiBaseUrl: '/api',
  useMocks: false,
  mqttUrl: (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/mqtt',
  mqttUser: 'fleet-ui',
  mqttPassword: '',
  mqttInterface: '',
  mqttVersion: '',
};
