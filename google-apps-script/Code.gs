/**
 * 门禁记录 → Google Sheet 接收端(Google Apps Script)
 * ----------------------------------------------------
 * 部署方法见项目根目录 README.md 第 5 步。
 * logger.py 和 esp32_bridge.ino 会把每次刷卡 POST 成 JSON 打到这里,
 * 本脚本把它追加成表格的一行。
 */
const SHEET_NAME = 'Sheet1';   // 改成你表格底部标签页的名字

function doPost(e) {
  const d = JSON.parse(e.postData.contents);
  const sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME);
  // 第一次使用可以先手动在第一行填表头:
  // mac_time | device_unix | device_time | name | uid | result
  sheet.appendRow([d.mac_time, d.device_unix, d.device_time, d.name, d.uid, d.result]);
  return ContentService.createTextOutput('OK');
}
